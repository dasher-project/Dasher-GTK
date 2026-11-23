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

Runs from the repo root with no arguments; stdlib only.

  --fix   Rewrite the hard-pinned `commit:` lines (rust-tts-wrapper,
          DasherCore) to the submodule SHAs staged in the index, then
          check. For submodule bumps: `git add <submodule>`, run with
          --fix, `git add` the manifest. The shared-deps bump workflow
          does exactly this.
"""

import re
import subprocess
import sys
from pathlib import Path

MANIFEST = Path("packaging/flatpak/org.alternativeinterface.dasher.yaml")

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


def main() -> int:
    failed = False

    if "--fix" in sys.argv[1:]:
        for name, path, url_fragment in HARD_PINS:
            sha = staged_submodule_sha(path)
            if fix_manifest_pin(url_fragment, sha):
                print(f"fixed: {name} manifest pin -> {sha[:12]}")
        # The check below compares against HEAD, which a not yet committed
        # bump has not reached; the fix itself is the result here.
        return 0

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
