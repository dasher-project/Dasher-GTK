#!/usr/bin/env python3
"""Verify the Flatpak manifest's submodule pins follow the submodules.

The Flatpak manifest (packaging/flatpak/org.alternativeinterface.dasher.yaml)
builds from the working tree but clones the submodules from their own git
sources with hand-maintained pins. When a submodule bumps and the manifest
pin is forgotten, the Flatpak build compiles a stale dependency against the
new app code — exactly how the v6.0.2 publish failed (rust-tts-wrapper
pinned at 1979957 while the C++ expected the 1.16.2 FFI surface).

Checks:
  HARD FAIL  rust-tts-wrapper: the manifest's `commit:` must equal the
             submodule's recorded gitlink SHA.
  WARNING    SDL: the manifest pins a tag; if the submodule has moved past
             it the build still works, but the log says so.

Runs from the repo root with no arguments; stdlib only.
"""

import re
import subprocess
import sys
from pathlib import Path

MANIFEST = Path("packaging/flatpak/org.alternativeinterface.dasher.yaml")


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


def manifest_git_pin(url_fragment: str) -> tuple[str | None, str | None]:
    """The commit (or tag) the manifest pins for the git source whose url
    contains `url_fragment`. Returns (commit, tag); at most one is set."""
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

    # ── rust-tts-wrapper: hard equality with the submodule pin ──────
    subm = submodule_sha("rust-tts-wrapper")
    commit, _ = manifest_git_pin("AACTools/rust-tts-wrapper")
    if commit is None:
        print(
            "::error::No commit: pin found for the rust-tts-wrapper git source in "
            f"{MANIFEST} — the Flatpak build would clone its default branch. "
            "Pin it to the submodule SHA."
        )
        failed = True
    elif commit != subm:
        print(
            f"::error::Flatpak manifest pins rust-tts-wrapper at {commit[:12]} but the "
            f"submodule is at {subm[:12]}. The Flatpak leg would build a stale "
            "wrapper against the current app code (this is how the v6.0.2 publish "
            "failed). Update the manifest's commit to the submodule SHA."
        )
        failed = True
    else:
        print(f"ok: rust-tts-wrapper manifest pin == submodule ({subm[:12]})")

    # ── SDL: tag pin — warn when the submodule has moved past it ────
    _, tag = manifest_git_pin("libsdl-org/SDL")
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
