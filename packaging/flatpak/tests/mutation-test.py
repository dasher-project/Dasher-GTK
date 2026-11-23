#!/usr/bin/env python3
"""Show that smoke-test.py notices regressions: break a copy of a build in
one way at a time and expect the check that guards against it to fail.

    packaging/flatpak/tests/mutation-test.py BUILD_DIR
    packaging/flatpak/tests/mutation-test.py BUILD_DIR --host-checks
    packaging/flatpak/tests/mutation-test.py BUILD_DIR --only session-bus,stray-archive
    packaging/flatpak/tests/mutation-test.py --list

A smoke test that is green says little until it has been seen red. Each
mutation below is a regression somebody could introduce in the manifest,
the metainfo or the build: an extra permission, a licence file that is no
longer installed, a static archive that cleanup misses, a prebuilt speech
library, a screenshot URL on a branch. The build directory itself is never
touched: it is copied to a scratch directory (about 220 MB), each mutation
is applied to the copy, the checks named for it are run, and the copy is
put back. One line per mutation: CAUGHT or MISSED. The exit status is 1
when a mutation was missed or the unmodified copy does not pass.

Like smoke-test.py it stays inside the sandbox unless told otherwise: the
checks that reach outside (the linter, the sound server, /dev/input; see
the top of smoke-test.py) are not expected to fail, and a mutation that
only they can catch is left out, until `--host-checks` is given.

Not part of CI (it needs a finished build and takes under a minute); run
it after changing smoke-test.py. The checks that need no sandbox are
covered the same way on every pull request by
Scripts/tests/test_flatpak_smoke.py, with made-up build directories.

Stdlib only.
"""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SMOKE_TEST = HERE / "smoke-test.py"
APP_ID = "org.alternativeinterface.dasher"
LICENCES = f"files/share/licenses/{APP_ID}"
METAINFO = f"files/share/metainfo/{APP_ID}.metainfo.xml"
DESKTOP = f"files/share/applications/{APP_ID}.desktop"
# The build directory under test (set in main); only ever read.
ORIGINAL = None


def edit(build, relative, old, new, count=1):
    """Replace `old` (a regular expression) in a text file of the copy."""
    path = build / relative
    text = path.read_text()
    changed, n = re.subn(old, new, text, count=count)
    if n == 0:
        raise SystemExit(f"mutation does not apply: {old!r} not found in {relative}")
    path.write_text(changed)
    return [relative]


def patch(build, relative, old, new):
    """Replace bytes of equal length in a binary file of the copy."""
    if len(old) != len(new):
        raise SystemExit("patch needs replacement bytes of the same length")
    path = build / relative
    data = path.read_bytes()
    if old not in data:
        raise SystemExit(f"mutation does not apply: {old!r} not found in {relative}")
    path.write_bytes(data.replace(old, new, 1))
    return [relative]


def remove(build, relative):
    path = build / relative
    if path.is_dir():
        shutil.rmtree(path)
    else:
        path.unlink()
    return [relative]


def create(build, relative, content=b"x"):
    path = build / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    # Report the topmost directory that was created, so restoring removes it.
    top = Path(relative)
    while len(top.parts) > 1 and not (ORIGINAL / top.parent).exists():
        top = top.parent
    return [top.as_posix()]


# (name, what it stands for, the mutation, checks that have to fail)
MUTATIONS = [
    (
        "session-bus",
        "finish-args gains --socket=session-bus",
        lambda b: edit(b, "metadata", r"(?m)^sockets=", "sockets=session-bus;"),
        ["permissions"],
    ),
    (
        "host-filesystem",
        "finish-args gains --filesystem=host",
        lambda b: edit(b, "metadata", r"(?m)^filesystems=", "filesystems=host;"),
        ["permissions"],
    ),
    (
        "system-bus-name",
        "finish-args gains a system bus name",
        lambda b: edit(
            b,
            "metadata",
            r"\n\[Extension ",
            "\n[System Bus Policy]\norg.freedesktop.login1=talk\n\n[Extension ",
        ),
        ["permissions"],
    ),
    (
        "other-bus-name",
        "--own-name is changed to a name the app does not take",
        lambda b: edit(b, "metadata", r"(?m)^org\.dasher\.gtk=own$", "org.dasher.other=own"),
        ["permissions", "bus-name"],
    ),
    (
        "no-sound-socket",
        "--socket=pulseaudio is dropped",
        lambda b: edit(b, "metadata", r"pulseaudio;", ""),
        ["permissions", "audio-device"],
    ),
    (
        "no-devices",
        "--device=all is dropped",
        lambda b: edit(b, "metadata", r"(?m)^devices=all;$", "devices="),
        ["permissions", "input-devices"],
    ),
    (
        "licence-dropped",
        "ONNX Runtime's ThirdPartyNotices.txt is not installed",
        lambda b: remove(b, f"{LICENCES}/onnxruntime/ThirdPartyNotices.txt"),
        ["files", "licences"],
    ),
    (
        "dependency-licence-dropped",
        "a sherpa-onnx dependency has no licence directory",
        lambda b: remove(b, f"{LICENCES}/sherpa-onnx/kissfft"),
        ["licences"],
    ),
    (
        "new-dependency",
        "sherpa-onnx links a dependency nobody has looked at",
        lambda b: create(b, f"{LICENCES}/sherpa-onnx/newdep/LICENSE", b"some licence\n"),
        ["licences"],
    ),
    (
        "stray-archive",
        "a static archive survives cleanup",
        lambda b: create(b, "files/lib/libstray.a"),
        ["cleanup"],
    ),
    (
        "headers-left",
        "/app/include survives cleanup",
        lambda b: create(b, "files/include/stray.h"),
        ["cleanup"],
    ),
    (
        "build-prefix-left",
        "the build prefix of the speech libraries survives cleanup",
        lambda b: create(b, "files/sherpa-onnx/lib/pkgconfig/sherpa-onnx.pc"),
        ["cleanup"],
    ),
    (
        "screenshot-on-branch",
        "the screenshot URL names a branch",
        lambda b: edit(
            b, METAINFO, r"Dasher-GTK/[0-9a-f]{40}/packaging", "Dasher-GTK/main/packaging"
        ),
        ["screenshot"],
    ),
    (
        "screenshot-size",
        "the declared screenshot size is not the image's",
        lambda b: edit(b, METAINFO, r'width="[0-9]+"', 'width="1234"'),
        ["screenshot"],
    ),
    (
        "release-not-bumped",
        "the newest metainfo release is not the version of the build",
        lambda b: edit(b, METAINFO, r'<release version="[^"]+"', '<release version="99.0.0"'),
        ["release"],
    ),
    (
        "desktop-exec",
        "the desktop file starts another command",
        lambda b: edit(b, DESKTOP, r"(?m)^Exec=.*$", "Exec=dasher-gtk"),
        ["desktop-file"],
    ),
    (
        "metainfo-id",
        "the metainfo names another app",
        lambda b: edit(b, METAINFO, r"<id>[^<]+</id>", "<id>org.example.other</id>"),
        ["metainfo"],
    ),
    (
        "icon-missing",
        "the scalable icon is not installed",
        lambda b: remove(b, f"files/share/icons/hicolor/scalable/apps/{APP_ID}.svg"),
        ["files"],
    ),
    (
        "library-missing",
        "libdasher.so is not installed",
        lambda b: remove(b, "files/libdasher.so"),
        ["files", "linkage", "start"],
    ),
    (
        "build-machine-rpath",
        "the binary's RPATH points at a directory of the build machine",
        lambda b: patch(b, "files/dasher", b"\0$ORIGIN\0", b"\0/tmp/xy\0"),
        ["rpath"],
    ),
    (
        "prebuilt-onnxruntime",
        "the speech library carries an ONNX Runtime built from a git checkout",
        lambda b: patch(
            b,
            "files/librust_tts_wrapper.so",
            b"git-branch=, git-commit-id=,",
            b"git-branch=,git-commit-id=3,",
        ),
        ["speech-from-source"],
    ),
    (
        "speech-library-broken",
        "librust_tts_wrapper.so is not a loadable library",
        lambda b: create(b, "files/librust_tts_wrapper.so", b"\x7fELF" + b"\0" * 60),
        ["linkage", "speech-library", "start"],
    ),
    (
        "error-on-stderr",
        "the app reports an error on stderr that never reaches GLib's log",
        lambda b: edit(
            b,
            "files/bin/dasher",
            r"(?m)^exec ",
            'echo "InputManager: SDL joystick init failed: no devices" >&2\nexec ',
        ),
        ["log"],
    ),
    (
        "persistent-directory",
        "finish-args gains --persist, which no sandbox of the test can switch off",
        lambda b: edit(b, "metadata", r"(?m)^\[Context\]$", "[Context]\npersistent=.dasher;"),
        ["permissions"],
    ),
    (
        "launcher-broken",
        "the launcher starts a binary that is not there",
        lambda b: edit(b, "files/bin/dasher", r"exec /app/dasher", "exec /app/dasher-missing"),
        ["files", "help", "start", "bus-name"],
    ),
    (
        "engine-data-missing",
        "the engine's Data directory is not installed",
        lambda b: remove(b, "files/Data"),
        ["files"],
    ),
    (
        "lint-new-error",
        "the build has a permission that flatpak-builder-lint rejects",
        # finish-args-arbitrary-dbus-access, an error that is not expected.
        lambda b: edit(b, "metadata", r"(?m)^sockets=", "sockets=session-bus;"),
        ["lint-builddir"],
    ),
]


def host_checks():
    """The checks of smoke-test.py that reach outside the sandbox: the ones
    its --list tags `host`."""
    done = subprocess.run(
        [sys.executable, str(SMOKE_TEST), "--list"], capture_output=True, text=True, check=True
    )
    return {
        line.split()[0]
        for line in done.stdout.splitlines()
        if re.search(r"\[[^\]]*\bhost\b[^\]]*\]$", line)
    }


def without_host_checks(mutations, host):
    """(mutations with the checks in `host` taken out of what they expect,
    names of the mutations that are left with no check at all)."""
    kept, left_out = [], []
    for name, summary, mutate, checks in mutations:
        inside = [check for check in checks if check not in host]
        if inside:
            kept.append((name, summary, mutate, inside))
        else:
            left_out.append(name)
    return kept, left_out


def run_smoke(build, checks, extra):
    """{check: "PASS" | "FAIL" | "SKIP"} for a smoke-test run on `build`."""
    cmd = [sys.executable, str(SMOKE_TEST), str(build), "--check", ",".join(checks)] + extra
    done = subprocess.run(cmd, capture_output=True, text=True)
    status = {}
    for line in done.stdout.splitlines():
        m = re.match(r"(PASS|FAIL|SKIP)  ([\w-]+):", line)
        if m:
            status[m.group(2)] = m.group(1)
    return status, done.stdout


def restore(build, touched):
    for relative in touched:
        target, source = build / relative, ORIGINAL / relative
        if target.is_dir() and not target.is_symlink():
            shutil.rmtree(target)
        elif target.exists() or target.is_symlink():
            target.unlink()
        if source.is_dir():
            shutil.copytree(source, target, symlinks=True)
        elif source.exists():
            shutil.copy2(source, target)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], allow_abbrev=False)
    parser.add_argument(
        "build_dir", nargs="?", help="flatpak-builder build directory (it is copied, never changed)"
    )
    parser.add_argument("--list", action="store_true", help="list the mutations and exit")
    parser.add_argument(
        "--only", default="", metavar="NAMES", help="run only these mutations (comma-separated)"
    )
    parser.add_argument(
        "--scratch", metavar="DIR", help="where to put the copy (default: the temporary directory)"
    )
    parser.add_argument(
        "--host-checks",
        action="store_true",
        help="also expect the checks that reach outside the sandbox to fail (passed on to "
        "smoke-test.py: the linter with network, the sound server, /dev/input)",
    )
    parser.add_argument(
        "--no-lint",
        action="store_true",
        help="with --host-checks: leave out the mutation that needs flatpak-builder-lint",
    )
    parser.add_argument(
        "smoke_args",
        nargs="*",
        help="after `--`: passed on to smoke-test.py (for example --voice-cache DIR)",
    )
    options = parser.parse_args()
    if options.list:
        for name, summary, _, checks in MUTATIONS:
            print(f"{name:<28} {summary}  -> {', '.join(checks)}")
        return 0
    if not options.build_dir:
        parser.error("BUILD_DIR is required")
    only = [n for n in options.only.split(",") if n]
    unknown = set(only) - {m[0] for m in MUTATIONS}
    if unknown:
        parser.error(f"unknown mutation(s): {', '.join(sorted(unknown))}")
    selected = [m for m in MUTATIONS if (not only or m[0] in only)]
    if options.no_lint:
        selected = [m for m in selected if "lint-builddir" not in m[3]]
    left_out = []
    if options.host_checks:
        options.smoke_args = ["--host-checks", *options.smoke_args]
    else:
        selected, left_out = without_host_checks(selected, host_checks())
    if not selected:
        # smoke-test.py takes an empty --check as "all checks".
        parser.error(
            "no mutation left to run"
            + (f" ({', '.join(left_out)} need --host-checks)" if left_out else "")
        )

    global ORIGINAL
    ORIGINAL = original = Path(options.build_dir).resolve()
    scratch = Path(tempfile.mkdtemp(prefix="dasher-mutation-", dir=options.scratch))
    missed = []
    try:
        print(f"copying {original} to {scratch} ...", flush=True)
        build = scratch / "build"
        shutil.copytree(original, build, symlinks=True)

        # The unmodified copy has to pass, or a failure below proves nothing.
        checks = sorted({check for m in selected for check in m[3]})
        baseline, output = run_smoke(build, checks, options.smoke_args)
        not_passing = sorted(c for c in checks if baseline.get(c) != "PASS")
        if not_passing:
            print(output)
            print(f"BASELINE: {', '.join(not_passing)} do not pass on the unmodified copy")
            return 1
        print(f"baseline: {len(checks)} checks pass on the unmodified copy")
        if left_out:
            print(f"left out, only a check behind --host-checks notices: {', '.join(left_out)}")
        print()

        for name, summary, mutate, expected in selected:
            touched = mutate(build)
            try:
                status, output = run_smoke(build, expected, options.smoke_args)
            finally:
                restore(build, touched)
            caught = [c for c in expected if status.get(c) == "FAIL"]
            verdict = "CAUGHT" if caught == expected else "MISSED"
            print(f"{verdict}  {name}: {summary}")
            for check in expected:
                reason = ""
                if status.get(check) == "FAIL":
                    lines = output.splitlines()
                    at = next(i for i, l in enumerate(lines) if l.startswith(f"FAIL  {check}:"))
                    reason = lines[at + 1].strip()[:150] if at + 1 < len(lines) else ""
                print(f"        {check}: {status.get(check, 'not run')}  {reason}")
            if verdict == "MISSED":
                missed.append(name)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    print(f"\n{len(selected) - len(missed)} of {len(selected)} mutations caught")
    if missed:
        print(f"Missed: {', '.join(missed)}")
    return 1 if missed else 0


if __name__ == "__main__":
    sys.exit(main())
