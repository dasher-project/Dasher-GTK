#!/usr/bin/env python3
"""GTK#98 review I3: content assertion for the generated po files.

The freshness gate (git diff --exit-code) only proves the committed files
REGENERATE identically — reproducible garbage passes it (review C1: the
corrupted 11k-msgstr regeneration sailed through). This script checks
CONTENT: every translated msgstr must equal the catalogue's translation for
that locale, modulo the msgfmt newline top-up the generator applies.

Exits 1 on any mismatch. Run after Scripts/generate-from-shared.py.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SHARED = os.path.join(REPO, "shared-resources", "ui-strings.json")
PO_DIR = os.path.join(REPO, "po")

# Catalogue keys whose en form differs from the code's msgid (mirrors the
# generator's MSGID_TO_KEY).
MSGID_TO_KEY = {
    "Open Text File": "open_text_file",
    "Save Text File": "save_text_file",
}


def po_unescape(s):
    return s.replace('\\"', '"').replace("\\\\", "\\").replace("\\n", "\n")


def main():
    shared = json.load(open(SHARED))["shared"]
    # en-text -> entry (the generator keys msgids by English text)
    # Mirror the GENERATOR's key selection exactly: sorted key order,
    # first-wins dedup for duplicate en texts (seen_msgids in the generator),
    # then the msgid-form overrides. A checker that picks a different key for
    # duplicate-text msgids than the generator did is a false-positive
    # machine (107 of them, at first run).
    by_en = {}
    seen = set()
    for key in sorted(shared.keys()):
        entry = shared[key]
        en = entry.get("en", "")
        if not en or en in seen:
            continue
        seen.add(en)
        by_en[en] = (key, entry)
    for msgid, key in MSGID_TO_KEY.items():
        if key in shared:
            by_en[msgid] = (key, shared[key])

    mismatches = []
    checked = 0
    english_fallbacks = []

    # Review loop-2 hole 2: coverage direction — every msgid the code uses
    # (_() in src/**.cpp/h) must exist in the po, or it's English-forever
    # and invisible to every other gate (2 live victims found: the
    # re-anchor and Select All tooltips).
    code_msgids = set()
    src_root = os.path.join(REPO, "src")
    for dirpath, _dirs, files in os.walk(src_root):
        for fn in files:
            if not fn.endswith((".cpp", ".h")):
                continue
            text = open(os.path.join(dirpath, fn), encoding="utf-8", errors="replace").read()
            code_msgids.update(re.findall(r'_\("((?:[^"\\]|\\.)*)"\)', text))
    po_msgids = set()
    for locale in open(os.path.join(PO_DIR, "LINGUAS")).read().split():
        po = open(os.path.join(PO_DIR, f"{locale}.po")).read()
        po_msgids.update(re.findall(r'msgid "((?:[^"\\]|\\.)*)"', po))
    missing = sorted(m for m in code_msgids if m and m not in po_msgids and not m.startswith("%"))
    if missing:
        for m in missing[:10]:
            print(f"  UNCATALOGUED: {m!r} is used in code but in no po file")
        print(f"FAIL: {len(missing)} code msgid(s) missing from the catalogue — English-forever")
        sys.exit(1)
    print(f"code coverage OK: {len(code_msgids)} _() msgids all present in po")
    for locale in open(os.path.join(PO_DIR, "LINGUAS")).read().split():
        po_path = os.path.join(PO_DIR, f"{locale}.po")
        po = open(po_path).read()
        for m in re.finditer(r'msgid "((?:[^"\\]|\\.)*)"\nmsgstr "((?:[^"\\]|\\.)*)"', po):
            msgid, msgstr = po_unescape(m.group(1)), po_unescape(m.group(2))
            hit = by_en.get(msgid)
            if not hit or not msgstr:
                continue  # untranslated or app-side msgid
            _key, entry = hit
            # Review loop-2 hole 1: an English msgstr under a locale the
            # catalogue doesn't translate is by definition WRONG (the
            # generator emits "" for untranslated) — never fall back to en.
            want = entry.get(locale, "")
            # The generator tops up trailing newlines to match the msgid;
            # compare on the newline-stripped tails.
            if want and msgstr.rstrip("\n") != want.rstrip("\n"):
                mismatches.append(f"{locale}: {msgid[:40]!r} -> {msgstr[:40]!r} (catalogue: {want[:40]!r})")
            elif not want and msgstr:
                english_fallbacks.append(f"{locale}: {msgid[:40]!r} is English text but the catalogue doesn't translate it")
            checked += 1

    print(f"{checked} translated entries checked against the catalogue")
    if english_fallbacks:
        for e in english_fallbacks[:10]:
            print("  FALLBACK:", e)
        print(f"FAIL: {len(english_fallbacks)} English-fallback msgstr(s) — the generator should emit empty for these")
        sys.exit(1)
    if mismatches:
        for e in mismatches[:10]:
            print(" ", e)
        print(f"FAIL: {len(mismatches)} msgstr(s) differ from the catalogue")
        sys.exit(1)
    print("content OK: every msgstr matches the catalogue")


if __name__ == "__main__":
    main()
