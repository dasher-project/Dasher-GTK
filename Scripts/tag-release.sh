#!/usr/bin/env bash
# Tag a Dasher-GTK release — with the checks that are easy to forget.
#
# The tag-vs-metainfo consistency check (validate-metadata.yml) hard-fails a
# tag push when packaging/org.alternativeinterface.dasher.metainfo.xml doesn't
# carry a matching <release> entry, and publish.yml refuses to ship packages
# from such a tag. This script makes the mistake impossible to make quietly:
# it refuses to tag unless the entry is already in place (plus the usual
# release hygiene).
#
# Usage: Scripts/tag-release.sh v0.2.12
set -euo pipefail

die() { echo "error: $*" >&2; exit 1; }

[[ $# -eq 1 ]] || die "usage: $0 vX.Y.Z"
version="$1"
[[ "$version" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "version must look like v0.2.12, got '$version'"
bare="${version#v}"
metainfo="packaging/org.alternativeinterface.dasher.metainfo.xml"

# 1. Run from the repo root (script may be invoked from anywhere)
root="$(git rev-parse --show-toplevel 2>/dev/null)" || die "not a git checkout"
cd "$root"

# 2. On main, clean tree
branch="$(git rev-parse --abbrev-ref HEAD)"
[[ "$branch" == "main" ]] || die "on branch '$branch' — tag releases from main"
[[ -z "$(git status --porcelain --untracked-files=no)" ]] || die "working tree has uncommitted changes"

# 2b. Local main is current — a stale local main tags the wrong source and
# the tag push publishes it (review finding: exactly this happened the day
# this script was written, so it's a real failure mode, not theoretical).
git fetch origin main --quiet
local_sha="$(git rev-parse main)"
remote_sha="$(git rev-parse origin/main)"
[[ "$local_sha" == "$remote_sha" ]] ||
    die "local main ($(git rev-parse --short main)) is not origin/main ($(git rev-parse --short origin/main)) — pull/rebase first"

# 3. Tag not already taken
git rev-parse -q --verify "refs/tags/$version" >/dev/null && die "tag $version already exists"

# 4. Metainfo carries this release entry (the one everyone forgets)
[[ -f "$metainfo" ]] || die "$metainfo not found"
newest="$(grep -m1 -oP '(?<=<release version=")[^"]+' "$metainfo")"
[[ "$newest" == "$bare" ]] || die "newest metainfo <release> is '$newest', expected '$bare'.
Add the entry to $metainfo (top of <releases>), commit, then re-run. See
README § Packaging & releases — 'Cutting a release'."

# 5. Submodule pinned to a DasherCore tag, not a floating commit
sub_desc="$(git -C DasherCore describe --tags --exact-match 2>/dev/null || true)"
[[ -n "$sub_desc" ]] || die "DasherCore submodule is not pinned at a tag ($(git -C DasherCore describe --tags --always 2>/dev/null || echo 'unknown')). Tag DasherCore first and bump the pin."

echo "✓ branch main, tree clean"
echo "✓ tag $version is free"
echo "✓ metainfo has <release version=\"$bare\">"
echo "✓ DasherCore pinned at $sub_desc"

git tag -a "$version" -m "Dasher-GTK $version"
echo "Tagged $version at $(git rev-parse --short HEAD)."
echo "Push it: git push origin $version"
