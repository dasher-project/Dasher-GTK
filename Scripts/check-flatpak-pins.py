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
             be a source in the manifest.

Runs from the repo root with no arguments; stdlib only.

  --fix   Rewrite the hard-pinned `commit:` lines (rust-tts-wrapper,
          DasherCore) to the submodule SHAs staged in the index. For
          submodule bumps: `git add <submodule>`, run with --fix,
          `git add` the manifest. The shared-deps bump workflow does
          exactly this. --fix cannot regenerate the offline cargo
          inputs: when the staged rust-tts-wrapper is not the commit
          the lock was generated for, it says so and exits non-zero,
          and the check keeps failing until they are regenerated.
"""

import json
import re
import subprocess
import sys
import tomllib
import urllib.error
import urllib.request
from pathlib import Path

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


def fix_manifest_pin(url_fragment: str, sha: str) -> bool:
    """Point the `commit:` of the git source whose url contains
    `url_fragment` at `sha`. Returns True when the manifest changed."""
    text = MANIFEST.read_text()
    starts = [m.end() for m in re.finditer(r"(?m)^\s*-\s+type:\s*git\s*$", text)]
    for start, end in zip(starts, starts[1:] + [len(text)]):
        block = text[start:end]
        m_url = re.search(r"(?m)^\s*url:\s*(\S+)", block)
        if not m_url or url_fragment not in m_url.group(1):
            continue
        m_commit = re.search(r"(?m)^(\s*commit:\s*)([0-9a-f]{40})", block)
        if not m_commit:
            raise SystemExit(f"error: no commit: line to fix for {url_fragment} in {MANIFEST}")
        if m_commit.group(2) == sha:
            return False
        MANIFEST.write_text(
            text[: start + m_commit.start(2)] + sha + text[start + m_commit.end(2) :]
        )
        return True
    raise SystemExit(f"error: no git source for {url_fragment} in {MANIFEST}")


def manifest_git_pin(url_fragment: str) -> tuple[str | None, str | None]:
    """The commit and tag the manifest pins for the git source whose url
    contains `url_fragment`. Returns (commit, tag); either may be None."""
    text = MANIFEST.read_text()
    # Every git source block: "- type: git" followed by url/commit/tag lines
    # (possibly with other keys between). Walk the source blocks in order.
    blocks = re.split(r"(?m)^\s*-\s+type:\s*git\s*$", text)
    for block in blocks[1:]:
        m_url = re.search(r"(?m)^\s*url:\s*(\S+)", block)
        if not m_url or url_fragment not in m_url.group(1):
            continue
        m_commit = re.search(r"(?m)^\s*commit:\s*([0-9a-f]{40})", block)
        m_tag = re.search(r"(?m)^\s*tag:\s*(\S+)", block)
        return (m_commit.group(1) if m_commit else None, m_tag.group(1) if m_tag else None)
    return (None, None)


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


def remote_tag_commit(url: str, tag: str) -> str | None:
    """The commit `tag` points at in the remote `url`, or None when the tag
    does not exist (or the remote cannot be reached). An annotated tag is
    peeled to its commit."""
    result = subprocess.run(
        ["git", "ls-remote", url, f"refs/tags/{tag}", f"refs/tags/{tag}^{{}}"],
        capture_output=True,
        text=True,
    )
    refs = {}
    for line in result.stdout.splitlines():
        sha, _, ref = line.partition("\t")
        refs[ref] = sha
    return refs.get(f"refs/tags/{tag}^{{}}") or refs.get(f"refs/tags/{tag}")


def fetch_text(url: str) -> tuple[str | None, str | None]:
    """(text, None) for a readable URL; (None, error) for an HTTP error,
    which means the file is not there; (None, None) when the host cannot be
    reached at all, which callers let through with a warning."""
    try:
        with urllib.request.urlopen(url, timeout=30) as response:
            return response.read().decode(), None
    except urllib.error.HTTPError as e:
        return None, f"HTTP {e.code} {e.reason}"
    except OSError as e:  # no HTTP response: DNS, connection, timeout
        print(f"::warning::Could not reach {url} ({e}); the check that needs it was skipped.")
        return None, None


def check_speech_stack(sherpa_version: str) -> bool:
    """The sherpa-onnx and ONNX Runtime the manifest builds from source are
    the ones the locked sherpa-onnx-sys `sherpa_version` expects. Returns
    True when something is out of sync."""
    failed = False
    manifest = MANIFEST.read_text()
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
    # The checks further down read sherpa-onnx's files at the pinned commit,
    # so stop here when tag and commit do not belong together.
    remote = remote_tag_commit(url, tag)
    if remote is None:
        print(f"::error::sherpa-onnx tag {tag} pinned in the manifest does not resolve upstream.")
        return True
    if remote != commit:
        print(
            f"::error::Manifest pins sherpa-onnx tag {tag} with commit {commit} but the "
            f"tag resolves to {remote} upstream. flatpak-builder refuses that; update "
            "the commit together with the tag."
        )
        return True
    if not failed:
        print(f"ok: sherpa-onnx source is {tag} ({commit[:12]}), matching sherpa-onnx-sys")

    raw = f"https://raw.githubusercontent.com/{SHERPA_REPO}/{commit}"

    # ONNX Runtime: the onnxruntime module asserts the version of the source
    # it builds (`test "$(cat VERSION_NUMBER)" = <version>`); that version
    # must be the release sherpa-onnx is built against upstream.
    m_ort = re.search(r'VERSION_NUMBER\)" = ([0-9]+(?:\.[0-9]+)+)', manifest)
    if not m_ort:
        print(
            f"::error::{MANIFEST} does not state the ONNX Runtime version it builds "
            '(expected `test "$(cat VERSION_NUMBER)" = <version>` in the onnxruntime module).'
        )
        failed = True
    else:
        for arch, path in SHERPA_ORT_CMAKE.items():
            text, error = fetch_text(f"{raw}/{path}")
            if error:
                print(f"::error::{path} is not readable at sherpa-onnx {commit[:12]} ({error}).")
                failed = True
            elif text is not None:
                pinned = set(re.findall(r"onnxruntime-libs/releases/download/v([0-9.]+)/", text))
                if pinned != {m_ort.group(1)}:
                    print(
                        f"::error::sherpa-onnx {tag} is built against ONNX Runtime "
                        f"{', '.join(sorted(pinned)) or '(not found)'} on {arch} but the manifest "
                        f"builds {m_ort.group(1)}. Move the onnxruntime module (source, version "
                        "check and its cmake/deps.txt mirror files) to that release."
                    )
                    failed = True
                else:
                    print(f"ok: ONNX Runtime {m_ort.group(1)} is what sherpa-onnx {tag} pins ({arch})")

    # Dependency tarballs: cmake verifies them with these SHA256 values at
    # build time, so a missing or different file only fails in the (offline)
    # Flatpak build. Catch it here.
    sha256s = set(re.findall(r"(?m)^\s*sha256:\s*([0-9a-f]{64})\s*$", manifest))
    missing = []
    checked = 0
    for path in SHERPA_DEP_CMAKE:
        text, error = fetch_text(f"{raw}/{path}")
        if error:
            print(f"::error::{path} is not readable at sherpa-onnx {commit[:12]} ({error}).")
            failed = True
            continue
        if text is None:
            continue
        for name, digest in re.findall(r'set\(([\w-]+)_HASH\s+"SHA256=([0-9a-f]{64})"\)', text):
            checked += 1
            if digest not in sha256s:
                missing.append(f"{name} ({path}, sha256 {digest[:12]}...)")
    if missing:
        print(
            f"::error::sherpa-onnx {tag} pins dependency tarballs that are not sources in "
            f"{MANIFEST}: {', '.join(missing)}. The build is offline and cmake would fail to "
            "download them; add them next to the sherpa-onnx source (url and sha256 are in "
            "the named cmake file)."
        )
        failed = True
    elif checked:
        print(f"ok: the {checked} dependency tarballs sherpa-onnx {tag} pins are manifest sources")
    return failed


def check_cargo_inputs(wrapper_sha: str) -> bool:
    """The offline cargo inputs agree with each other and with the pinned
    rust-tts-wrapper. Returns True when something is out of sync."""
    failed = check_lock_commit(wrapper_sha)
    packages = tomllib.loads(CARGO_LOCK.read_text())["package"]
    versions = {p["name"]: p["version"] for p in packages}

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
        print(
            f"::warning::Could not reach GitHub to read rust-tts-wrapper's Cargo.toml at "
            f"{wrapper_sha[:12]} ({e}); the crate version in {CARGO_LOCK.name} was not checked."
        )
    else:
        locked = versions.get("rust-tts-wrapper")
        if locked != pinned:
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
    if tag is None:
        print("::error::No tag: pin found for the SDL git source — pin a release tag.")
        failed = True
    else:
        # Resolve the tag without a checkout: ask the submodule's remote.
        result = subprocess.run(
            ["git", "ls-remote", "https://github.com/libsdl-org/SDL.git", f"refs/tags/{tag}"],
            capture_output=True,
            text=True,
        )
        remote = result.stdout.split()[0] if result.stdout.strip() else None
        if remote is None:
            print(f"::error::SDL tag {tag} pinned in the manifest does not resolve upstream.")
            failed = True
        elif sdl_commit is None:
            print(
                f"::error::No commit: next to the SDL tag {tag} in {MANIFEST}. "
                f"Add `commit: {remote}` so a moved tag cannot change the build."
            )
            failed = True
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

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
